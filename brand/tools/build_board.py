#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Renders the PartyUp brand board (brand/preview/brand-board.png).

    PYTHONPATH=.tools python3 brand/tools/build_board.py
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), ".tools"))

import cairosvg                                       # noqa: E402
import build_assets as B                              # noqa: E402

INK, CHAR, BONE = B.INK, B.CHAR, B.BONE
FOG, LINE, EMBER = B.FOG, B.LINE, B.EMBER

BOARD_BG = "#0E0F11"
PANEL, PANEL_2 = "#15171A", "#101215"
EDGE = "#242830"

SANS, MONO, CJK = "Ubuntu", "DejaVu Sans Mono", "Noto Sans CJK SC"


def esc(s: str) -> str:
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def t(x, y, s, size=13, fill=FOG, family=SANS, weight="400", anchor="start",
      spacing=0.0, opacity=1.0) -> str:
    s = esc(s)
    return (f'<text x="{x:.2f}" y="{y:.2f}" font-family="{family}" font-weight="{weight}" '
            f'font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
            f'letter-spacing="{spacing}" opacity="{opacity}">{s}</text>')


def label(x, y, s, fill=FOG, size=12, anchor="start") -> str:
    return t(x, y, s, size=size, fill=fill, family=MONO, spacing=1.4, anchor=anchor)


def mono(x, y, s, fill=FOG, size=11, anchor="start", spacing=0.4) -> str:
    return t(x, y, s, size=size, fill=fill, family=MONO, anchor=anchor, spacing=spacing)


def frame(w, h, fill=PANEL, ticks=True) -> list[str]:
    g = [f'<rect width="{w}" height="{h}" fill="{fill}"/>',
         f'<rect x="0.5" y="0.5" width="{w-1}" height="{h-1}" fill="none" stroke="{EDGE}"/>']
    if ticks:
        for cx, cy in ((14, 14), (w - 14, 14), (14, h - 14), (w - 14, h - 14)):
            g.append(f'<path d="M{cx-4} {cy}H{cx+4}M{cx} {cy-4}V{cy+4}" stroke="{EDGE}" stroke-width="1"/>')
    return g


def display(s, cap, x, y, fill=BONE, track=-0.022):
    el, w = B.WM.element(s, cap, 0, 0, fill, track)
    return f'<g transform="translate({x:.2f},{y:.2f})">{el}</g>', w


# ------------------------------------------------------------------ panels --
def panel_cover(w, h):
    g = frame(w, h, PANEL_2)
    ms = 200
    g.append(B.place_mark((w - ms) / 2, h / 2 - ms / 2 - 56, ms))
    el, ww = display("PartyUp", 56, 0, 0)
    g.append(f'<g transform="translate({(w - ww) / 2:.2f},{h / 2 + ms / 2 - 40:.2f})">{el}</g>')
    g.append(label(26, h - 24, "IDENTITY / 01"))
    g.append(label(w - 26, h - 24, "v0.1 · 2026", anchor="end", fill=EMBER))
    return "".join(g)


def panel_construction(w, h):
    g = frame(w, h)
    g.append(label(26, 28, "CONSTRUCTION"))
    S = 320.0
    ox, oy = 46.0, (h - S) / 2 + 12
    step = S / 10
    for i in range(11):
        g.append(f'<line x1="{ox+i*step:.1f}" y1="{oy:.1f}" x2="{ox+i*step:.1f}" y2="{oy+S:.1f}" stroke="{LINE}" stroke-width="0.55"/>')
        g.append(f'<line x1="{ox:.1f}" y1="{oy+i*step:.1f}" x2="{ox+S:.1f}" y2="{oy+i*step:.1f}" stroke="{LINE}" stroke-width="0.55"/>')
    k = S / 1024.0
    for yy in (B.CAP_TOP + B.NUDGE_Y, B.CAP_BOT + B.NUDGE_Y):
        g.append(f'<line x1="{ox-9:.1f}" y1="{oy+yy*k:.1f}" x2="{ox+S:.1f}" y2="{oy+yy*k:.1f}" '
                 f'stroke="{EMBER}" stroke-width="0.8" stroke-dasharray="5 5" opacity="0.6"/>')
    cx, cy = B.STEM_R + B.NUDGE_X, B.BOWL_CY + B.NUDGE_Y
    for r, col, dash in ((B.BOWL_R, FOG, "2 4"), (B.COUNTER_R, EMBER, "3 3")):
        g.append(f'<circle cx="{ox+cx*k:.1f}" cy="{oy+cy*k:.1f}" r="{r*k:.1f}" fill="none" '
                 f'stroke="{col}" stroke-width="0.8" stroke-dasharray="{dash}" opacity="0.55"/>')
    g.append(f'<g transform="translate({ox:.2f},{oy:.2f}) scale({k:.6f})">'
             f'<path d="{B.p_path()}" fill="{BONE}" fill-rule="evenodd"/>'
             f'<path d="{B.counter_shape()}" fill="{EMBER}"/></g>')
    gx = w - 28
    g.append(label(gx, 26, "GRID 1024", anchor="end"))
    g.append(label(gx, 42, "CAP 700", anchor="end"))
    rows = [("cap height", "700"), ("stem", "140"), ("bowl r", "270"), ("counter", "130")]
    yy = 76
    for name, val in rows:
        g.append(mono(gx, yy, name, anchor="end"))
        g.append(t(gx, yy + 22, val, size=19, fill=BONE, family=MONO, anchor="end"))
        yy += 50
    g.append(t(gx, h - 56, "the counter carries", size=12, fill=FOG, anchor="end"))
    g.append(t(gx, h - 38, "the brand accent", size=12, fill=EMBER, anchor="end"))
    return "".join(g)


def panel_icon(w, h):
    g = frame(w, h)
    g.append(label(26, 28, "APP ICON"))
    big = 184
    g.append(B.place_mark(26, 52, big))
    x2 = 26 + big + 38
    for i, s in enumerate(("rounded square", "radius 21.9%", "one colour field", "knocked-out P")):
        g.append(mono(x2, 92 + i * 18, s))
    # size ladder, bottom-aligned
    base = 300
    g.append(mono(26, base - 20, "16 — 1024 px", size=10.5))
    xx = 26
    for s in (64, 48, 32, 24, 16):
        g.append(B.place_mark(xx, base + (64 - s), s))
        g.append(mono(xx + s / 2, base + 88, str(s), anchor="middle", size=10))
        xx += s + 24
    ty = h - 84
    g.append(f'<rect x="26" y="{ty}" width="{w-52}" height="50" rx="10" fill="{PANEL_2}" stroke="{EDGE}"/>')
    g.append(B.place_mark(26 + 13, ty + 8, 34))
    for i in range(3):
        g.append(f'<rect x="{26+62+i*42}" y="{ty+14}" width="22" height="22" rx="6" fill="#2A2F38"/>')
    g.append(mono(w - 40, ty + 30, "TASKBAR 32", anchor="end", size=10))
    return "".join(g)


def panel_essence(w, h):
    g = frame(w, h, PANEL_2)
    el1, _ = display("Your server.", 56, 36, h / 2 - 48)
    el2, _ = display("Your people.", 56, 36, h / 2 + 26, EMBER)
    g.append(el1 + el2)
    g.append(t(38, h / 2 + 92, "一台服务器，几个频道，你的人。", size=16, fill=FOG, family=CJK))
    g.append(label(36, h - 24, "TAGLINE / EN + ZH"))
    return "".join(g)


def panel_color(w, h):
    g = frame(w, h)
    g.append(label(26, 28, "COLOR"))
    sw = [("INK", INK, "base", BONE), ("CHAR", "#1B1E23", "surface", BONE),
          ("BONE", BONE, "light", INK), ("FOG", FOG, "muted", INK),
          ("EMBER", EMBER, "accent", INK)]
    bw, gap, y = 86, 10, 54
    for i, (n, c, role, fg) in enumerate(sw):
        x = 26 + i * (bw + gap)
        g.append(f'<rect x="{x}" y="{y}" width="{bw}" height="142" rx="5" fill="{c}" stroke="{EDGE}"/>')
        g.append(mono(x + 9, y + 130, n, fill=fg, size=10))
        g.append(t(x, y + 168, c.upper(), size=11, fill=BONE, family=MONO))
        g.append(mono(x, y + 184, role, size=10, fill="#5E636B"))
    g.append(f'<line x1="26" y1="{h-92}" x2="{w-26}" y2="{h-92}" stroke="{LINE}"/>')
    g.append(mono(26, h - 64, "accent only for presence + focus", size=11))
    g.append(mono(26, h - 46, "never as a gradient · never on gradient", size=11))
    g.append(mono(26, h - 24, "bone/ink 17.3:1 · ember/ink 6.3:1", size=11))
    return "".join(g)


def panel_type(w, h):
    g = frame(w, h)
    g.append(label(26, 28, "TYPEFACE"))
    el, ww = display("PartyUp", 72, 26, 140)
    g.append(el)
    g.append(mono(26, 176, "Lato Black · SIL OFL 1.1", size=11))
    g.append(mono(26, 193, "wordmark shipped as outlines", size=11))
    g.append(f'<line x1="26" y1="{h-118}" x2="{w-26}" y2="{h-118}" stroke="{LINE}"/>')
    g.append(t(26, h - 76, "AaBbCcDdEeFfGg 0123", size=25, fill=BONE))
    g.append(mono(26, h - 48, "UI  system sans / Inter       LABELS  mono", size=10.5))
    g.append(mono(26, h - 26, "tracking −0.022 em · hand-kerning", size=10.5))
    return "".join(g)


def panel_app(w, h):
    g = frame(w, h, PANEL_2)
    g.append(label(26, 28, "IN PRODUCT"))
    wx, wy, ww, wh = 26, 48, w - 52, h - 82
    g.append(f'<rect x="{wx}" y="{wy}" width="{ww}" height="{wh}" rx="9" fill="{INK}" stroke="{EDGE}"/>')
    g.append(f'<line x1="{wx}" y1="{wy+46}" x2="{wx+ww}" y2="{wy+46}" stroke="{LINE}"/>')
    g.append(B.place_mark(wx + 14, wy + 10, 26))
    g.append(t(wx + 50, wy + 25, "general", size=13, fill=BONE))
    g.append(mono(wx + 50, wy + 38, "voice · screen · text", size=8))
    for i in range(3):
        g.append(f'<circle cx="{wx+ww-26-i*26}" cy="{wy+23}" r="3.2" fill="{EMBER if i==0 else LINE}"/>')
    yy = wy + 68
    rows = [("#", "general", 0), ("#", "build-log", 0), ("~", "voice-1", 2), ("~", "voice-2", 1)]
    for i, (ic, nm, n) in enumerate(rows):
        hl = i == 0
        if hl:
            g.append(f'<rect x="{wx+8}" y="{yy-14}" width="{ww*0.44}" height="25" rx="5" fill="#1E2126"/>')
        g.append(t(wx + 18, yy + 3, ic, size=11, fill=EMBER if i >= 2 else FOG, family=MONO))
        g.append(t(wx + 32, yy + 3, nm, size=11.5, fill=BONE if hl else FOG))
        for j in range(n):
            g.append(f'<circle cx="{wx+ww*0.44-15-j*9}" cy="{yy-3}" r="2.4" fill="{EMBER}" opacity="{0.9 if j==0 else 0.4}"/>')
        yy += 29
    mx = wx + ww * 0.5
    g.append(f'<line x1="{mx}" y1="{wy+46}" x2="{mx}" y2="{wy+wh}" stroke="{LINE}"/>')
    for i, (nm, msg) in enumerate((("kite", "party up in 5"), ("mora", "on my way"), ("kite", "sharing screen"))):
        by = wy + 84 + i * 52
        g.append(f'<circle cx="{mx+24}" cy="{by+6}" r="7" fill="{EMBER if i!=1 else "#2E333C"}"/>')
        g.append(mono(mx + 40, by + 2, nm, size=9))
        g.append(t(mx + 40, by + 17, msg, size=11, fill=BONE if i == 0 else FOG))
    return "".join(g)


def panel_atmosphere(w, h):
    g = [f'<defs><linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">'
         f'<stop offset="0" stop-color="#1B1411"/><stop offset="0.5" stop-color="#100F0E"/>'
         f'<stop offset="1" stop-color="#090A0B"/></linearGradient>'
         f'<radialGradient id="lamp" cx="0.62" cy="1.06" r="0.62">'
         f'<stop offset="0" stop-color="#FF7A45" stop-opacity="0.55"/>'
         f'<stop offset="0.45" stop-color="{EMBER}" stop-opacity="0.15"/>'
         f'<stop offset="1" stop-color="{EMBER}" stop-opacity="0"/></radialGradient></defs>']
    g.append(f'<rect width="{w}" height="{h}" fill="url(#sky)"/>')
    g.append(f'<rect width="{w}" height="{h}" fill="url(#lamp)"/>')
    g.append(f'<line x1="0" y1="{h*0.745:.1f}" x2="{w}" y2="{h*0.745:.1f}" stroke="#3A2E28" stroke-width="0.8"/>')
    for i in range(1, 9):
        g.append(f'<line x1="{w*i/9:.1f}" y1="{h*0.745:.1f}" x2="{w*i/9:.1f}" y2="{h}" stroke="#2A211D" stroke-width="0.6"/>')
    k = (h * 0.76) / 1024.0
    g.append(f'<g transform="translate({w*0.5-512*k:.1f},{h*0.07:.1f}) scale({k:.5f})">'
             f'<path d="{B.p_path()}" fill="{BONE}" fill-rule="evenodd" opacity="0.05"/></g>')
    for i in range(0, int(h), 3):
        g.append(f'<line x1="0" y1="{i}" x2="{w}" y2="{i}" stroke="#000" stroke-width="1" opacity="0.13"/>')
    g.append(f'<rect x="0.5" y="0.5" width="{w-1}" height="{h-1}" fill="none" stroke="{EDGE}"/>')
    g.append(t(26, h - 54, "the light stays on", size=22, fill=BONE))
    g.append(label(26, h - 26, "IMAGE DIRECTION / 02"))
    return "".join(g)


def panel_variants(w, h):
    g = frame(w, h)
    g.append(label(26, 28, "VARIANTS & RULES"))
    # three variants
    y = 52
    vs = [(B.mark_svg(), "PRIMARY", "ember field"),
          (B.mark_svg(bg=INK, p_fill=BONE, counter=EMBER), "ON DARK", "ink field"),
          (B.mark_svg(bg=None, p_fill=BONE, counter=None), "MONO", "one colour")]
    for i, (svg, name, note) in enumerate(vs):
        body = svg[svg.index(">", svg.index("<svg")) + 1:svg.rindex("</svg>")]
        x = 26 + i * 168
        g.append(f'<svg x="{x}" y="{y}" width="76" height="76" viewBox="0 0 1024 1024">{body}</svg>')
        g.append(mono(x, y + 96, name, size=10))
        g.append(mono(x, y + 111, note, size=9, fill="#5E636B"))
    # presence
    py = 232
    g.append(mono(26, py, "presence dots", size=11))
    for i, op in enumerate((1.0, 0.42)):
        g.append(f'<circle cx="{40+i*34}" cy="{py+18}" r="9" fill="{EMBER}" opacity="{op}"/>')
    g.append(f'<circle cx="{108}" cy="{py+18}" r="7.4" fill="none" stroke="{EMBER}" stroke-width="1.6"/>')
    g.append(mono(152, py + 22, "speaking · in channel · away", size=9.5, fill="#5E636B"))
    # rules
    g.append(f'<line x1="{w-236}" y1="{py-14}" x2="{w-26}" y2="{py-14}" stroke="{LINE}"/>')
    g.append(label(w - 26, py, "RULES", anchor="end"))
    rules = ["one colour field", "no gradient", "no outline / shadow", "min 16 px", "clear space ≥ ¼"]
    for i, r in enumerate(rules):
        g.append(mono(w - 26, py + 24 + i * 18, r, anchor="end", size=10.5, fill="#5E636B"))
    # mono wordmark
    el, ww = B.WM.element("PartyUp", 32, 0, 0, BONE)
    g.append(f'<g transform="translate(26,{h-58})">{el}</g>')
    g.append(mono(26, h - 26, "mono lockup — one colour, any surface", size=10))
    return "".join(g)


# ------------------------------------------------------------------- board --
def build() -> str:
    W, H = 2400, 1800
    PAD, GUT = 62, 18
    cw = (W - 2 * PAD - 2 * GUT) / 3
    ch = (H - 2 * PAD - 2 * GUT) / 3
    panels = [panel_cover, panel_construction, panel_icon,
              panel_essence, panel_color, panel_type,
              panel_app, panel_atmosphere, panel_variants]
    body = [f'<rect width="{W}" height="{H}" fill="{BOARD_BG}"/>']
    for i, fn in enumerate(panels):
        r, c = divmod(i, 3)
        x, y = PAD + c * (cw + GUT), PAD + r * (ch + GUT)
        body.append(f'<g transform="translate({x:.1f},{y:.1f})">{fn(int(cw), int(ch))}</g>')
    body.append(label(PAD, 38, "PARTYUP — BRAND IDENTITY SYSTEM", size=13))
    body.append(label(W - PAD, 38, "SELF-HOSTED VOICE / SCREEN / TEXT", anchor="end", size=13))
    body.append(f'<line x1="{PAD}" y1="50" x2="{W-PAD}" y2="50" stroke="{EDGE}"/>')
    body.append(label(PAD, H - 26, "BOARD 01 — 09"))
    body.append(label(W - PAD, H - 26, "RENDERED FROM SOURCE", anchor="end"))
    return B.svg(W, H, "".join(body))


def main() -> None:
    s = build()
    out_dir = os.path.join(ROOT, "preview")
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "brand-board.svg"), "w").write(s)
    cairosvg.svg2png(bytestring=s.encode(),
                     write_to=os.path.join(out_dir, "brand-board.png"),
                     output_width=2400, output_height=1800)
    print("board ok")


if __name__ == "__main__":
    main()
