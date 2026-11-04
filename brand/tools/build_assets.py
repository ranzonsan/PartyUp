#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PartyUp brand asset builder — single source of truth for the identity.

Every SVG / PNG / ICO under brand/ is produced by this file. Re-run after
changing any constant below.

    PYTHONPATH=.tools python3 brand/tools/build_assets.py

Build-time tooling only (never shipped with PartyUp):
  * cairosvg   LGPL-3.0   SVG -> PNG rasterisation
  * fontTools  MIT        glyph outlines for the wordmark (Lato Black, SIL OFL 1.1)

The wordmark is delivered as outlines, so no font binary is redistributed.
Output licensing: see brand/LICENSE-ASSETS.md.
"""
from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.dirname(ROOT)
sys.path.insert(0, os.path.join(REPO, ".tools"))

import cairosvg                                      # noqa: E402
from fontTools.ttLib import TTFont                   # noqa: E402
from fontTools.pens.svgPathPen import SVGPathPen     # noqa: E402
from fontTools.pens.transformPen import TransformPen  # noqa: E402

# ---------------------------------------------------------------- palette --
INK, CHAR, BONE = "#0B0C0E", "#16181C", "#F4F1EA"
FOG, LINE = "#8A8D93", "#2A2E35"
EMBER, EMBER_DEEP, EMBER_SOFT = "#FF5A2B", "#D93A11", "#FF8A63"

FONT_PATH = "/usr/share/fonts/truetype/lato/Lato-Black.ttf"
WORD = "PartyUp"

# --------------------------------------------------------------- geometry --
# The mark is a custom geometric P on a 1024x1024 grid.
CAP_TOP, CAP_BOT = 150.0, 850.0          # cap height 700
STEM_L, STEM_R = 280.0, 420.0            # stem width 140
BOWL_CY, BOWL_R = 420.0, 270.0           # circular bowl, outer radius 270
COUNTER_R = BOWL_R - (STEM_R - STEM_L)   # 130
NUDGE_X, NUDGE_Y = 27.0, 12.0            # optical centring in the canvas
TILE_R = 224.0                           # app-icon corner radius (21.9%)


def p_path(dx: float = 0.0, dy: float = 0.0) -> str:
    """P silhouette with the counter knocked out (fill-rule: evenodd)."""
    dx, dy = dx + NUDGE_X, dy + NUDGE_Y
    t, b = CAP_TOP + dy, CAP_BOT + dy
    sl, sr = STEM_L + dx, STEM_R + dx
    by, r, cr = BOWL_CY + dy, BOWL_R, COUNTER_R
    outer = (f"M{sl:.2f} {b:.2f}L{sl:.2f} {t:.2f}L{sr:.2f} {t:.2f}"
             f"A{r} {r} 0 0 1 {sr:.2f} {by + r:.2f}L{sr:.2f} {b:.2f}Z")
    inner = f"M{sr:.2f} {by - cr:.2f}A{cr} {cr} 0 0 1 {sr:.2f} {by + cr:.2f}Z"
    return f"{outer}{inner}"


def p_solid(dx: float = 0.0, dy: float = 0.0) -> str:
    """P silhouette with no counter (clip shape)."""
    dx, dy = dx + NUDGE_X, dy + NUDGE_Y
    t, b = CAP_TOP + dy, CAP_BOT + dy
    sl, sr = STEM_L + dx, STEM_R + dx
    by, r = BOWL_CY + dy, BOWL_R
    return (f"M{sl:.2f} {b:.2f}L{sl:.2f} {t:.2f}L{sr:.2f} {t:.2f}"
            f"A{r} {r} 0 0 1 {sr:.2f} {by + r:.2f}L{sr:.2f} {b:.2f}Z")


def counter_shape(dx: float = 0.0, dy: float = 0.0) -> str:
    dx, dy = dx + NUDGE_X, dy + NUDGE_Y
    sr = STEM_R + dx
    by, cr = BOWL_CY + dy, COUNTER_R
    return f"M{sr:.2f} {by - cr:.2f}A{cr} {cr} 0 0 1 {sr:.2f} {by + cr:.2f}Z"


# --------------------------------------------------------------- wordmark --
class Wordmark:
    """Lato Black, converted to outlines (no font dependency at render time).

    `text` defaults to the brand word; pass any string to set display type.
    """

    def __init__(self, font_path: str = FONT_PATH, tracking_em: float = -0.022):
        f = TTFont(font_path)
        self.cmap = f.getBestCmap()
        self.hmtx = f["hmtx"]
        self.upem = f["head"].unitsPerEm
        self.cap_units = f["OS/2"].sCapHeight or 1400
        self.tracking_em = tracking_em
        self.glyphset = f.getGlyphSet()
        self.track = tracking_em * self.upem

    # -- single string -------------------------------------------------------
    def path(self, text: str, cap_px: float, x: float, baseline_y: float,
             tracking_em: float | None = None) -> tuple[str, float]:
        s = cap_px / self.cap_units          # px per font unit
        track = self.track if tracking_em is None else tracking_em * self.upem
        parts, ox = [], x
        for ch in text:
            gn = self.cmap[ord(ch)]
            pen = SVGPathPen(self.glyphset)
            self.glyphset[gn].draw(TransformPen(pen, (s, 0, 0, -s, ox, baseline_y)))
            parts.append(pen.getCommands())
            ox += (self.hmtx[gn][0] + track) * s
        return "".join(parts), ox - x

    def element(self, text: str, cap_px: float, x: float, baseline_y: float,
                fill: str = BONE, tracking_em: float | None = None) -> tuple[str, float]:
        d, w = self.path(text, cap_px, x, baseline_y, tracking_em)
        return f'<path d="{d}" fill="{fill}"/>', w


WM = Wordmark()


# ------------------------------------------------------------------- SVG ---
def svg(w: int, h: int, body: str, view: str | None = None) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="{view or f"0 0 {w} {h}"}">{body}</svg>')


def mark_body(bg: str | None = EMBER, p_fill: str = INK, counter: str | None = None,
              tile_r: float = TILE_R, size: float = 1024.0) -> str:
    """Primary mark body on a `size` square grid: rounded tile + P."""
    body = ""
    if bg:
        body += f'<rect width="{size:.2f}" height="{size:.2f}" rx="{tile_r * size / 1024:.2f}" fill="{bg}"/>'
    body += f'<path d="{p_path()}" fill="{p_fill}" fill-rule="evenodd"/>'
    if counter:
        body += f'<path d="{counter_shape()}" fill="{counter}"/>'
    return body


def mark_svg(bg: str | None = EMBER, p_fill: str = INK, counter: str | None = None,
             tile_r: float = TILE_R) -> str:
    return svg(1024, 1024, mark_body(bg, p_fill, counter, tile_r))


def place_mark(x: float, y: float, size: float, **kw) -> str:
    """<g> placing the mark at (x,y) with edge length `size` on a 1024 design grid."""
    return (f'<g transform="translate({x:.2f},{y:.2f}) scale({size / 1024:.6f})">'
            f'{mark_body(**kw)}</g>')


def lockup_h_svg(mark_h: float = 128.0, cap_ratio: float = 0.36,
                 gap_ratio: float = 0.24, text_fill: str = BONE,
                 bg: str | None = None, mark: str | None = None) -> str:
    """Horizontal lockup. Mark box = mark_h; wordmark cap = cap_ratio * mark_h."""
    cap = mark_h * cap_ratio
    gap = mark_h * gap_ratio
    d, wm_w = WM.path(WORD, cap, 0, 0)
    total_w = mark_h + gap + wm_w
    pad = mark_h * 0.16
    W, H = total_w + pad * 2, mark_h * 1.6
    my = (H - mark_h) / 2
    baseline = (H + cap) / 2 - cap * 0.02
    body = ""
    if bg:
        body += f'<rect width="{W:.2f}" height="{H:.2f}" fill="{bg}"/>'
    body += (f'<g transform="translate({pad:.2f},{my:.2f}) scale({mark_h / 1024:.6f})">'
             f'{mark or mark_svg()}</g>')
    wm_el, _ = WM.element(WORD, cap, pad + mark_h + gap, baseline, text_fill)
    body += wm_el
    return svg(round(W), round(H), body)


def lockup_v_svg(mark_h: float = 200.0, cap_ratio: float = 0.34,
                 gap_ratio: float = 0.26, text_fill: str = BONE,
                 bg: str | None = None) -> str:
    cap = mark_h * cap_ratio
    gap = mark_h * gap_ratio
    d, wm_w = WM.path(WORD, cap, 0, 0)
    W = max(mark_h, wm_w) + mark_h * 0.24
    H = mark_h + gap + cap * 1.32 + mark_h * 0.12
    body = ""
    if bg:
        body += f'<rect width="{W:.2f}" height="{H:.2f}" fill="{bg}"/>'
    body += (f'<g transform="translate({(W - mark_h) / 2:.2f},{mark_h * 0.06:.2f}) '
             f'scale({mark_h / 1024:.6f})">{mark_svg()}</g>')
    baseline = mark_h * 1.06 + gap + cap
    wm_el, _ = WM.element(WORD, cap, (W - wm_w) / 2, baseline, text_fill)
    body += wm_el
    return svg(round(W), round(H), body)


def svg_text(x, y, s, size, fill=FOG, family="Ubuntu", weight="400",
             anchor="start", spacing=0) -> str:
    return (f'<text x="{x}" y="{y}" font-family="{family}" font-weight="{weight}" '
            f'font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
            f'letter-spacing="{spacing}">{s}</text>')


def splash_svg(w: int = 500, h: int = 300) -> str:
    """Installer splash (Velopack Setup). 5:3."""
    body = [f'<rect width="{w}" height="{h}" fill="{INK}"/>',
            f'<defs><radialGradient id="sg" cx="0.85" cy="0.95" r="0.6">'
            f'<stop offset="0" stop-color="{EMBER}" stop-opacity="0.20"/>'
            f'<stop offset="1" stop-color="{EMBER}" stop-opacity="0"/></radialGradient></defs>',
            f'<rect width="{w}" height="{h}" fill="url(#sg)"/>']
    ms = min(w, h) * 0.28
    body.append(f'<g transform="translate({w*0.076:.2f},{h/2-ms/2:.2f}) scale({ms/1024:.6f})">'
                f'{mark_body()}</g>')
    cap = ms * 0.36
    el, ww = WM.element(WORD, cap, 0, 0, BONE)
    x = w * 0.076 + ms + ms * 0.28
    body.append(f'<g transform="translate({x:.2f},{h/2+cap*0.34:.2f})">{el}</g>')
    body.append(f'<text x="{x:.2f}" y="{h/2+cap*0.34+cap*0.72:.2f}" font-family="DejaVu Sans Mono" '
                f'font-size="{max(6.0, ms*0.098):.1f}" fill="{FOG}" letter-spacing="1.3">'
                f'SELF-HOSTED VOICE / SCREEN / TEXT</text>')
    return svg(w, h, "".join(body))


# ----------------------------------------------------------------- export --
def write(path: str, content: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(content)


def png(svg_str: str, out: str, w: int, h: int | None = None) -> None:
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cairosvg.svg2png(bytestring=svg_str.encode(), write_to=out,
                     output_width=w, output_height=h or w)


def ico(png_files: list[str], out: str) -> None:
    """Multi-resolution .ico with PNG payloads (Vista+)."""
    blobs = [open(p, "rb").read() for p in png_files]
    n = len(blobs)
    header = struct.pack("<HHH", 0, 1, n)
    offset = 6 + 16 * n
    entries, data = [], b""
    for p, blob in zip(png_files, blobs):
        size = int(os.path.basename(p).split("-")[-1].split(".")[0])
        dim = 0 if size >= 256 else size
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                                   len(blob), offset + len(data)))
        data += blob
    with open(out, "wb") as fh:
        fh.write(header + b"".join(entries) + data)


# ------------------------------------------------------------------ main ---
def main() -> None:
    src = os.path.join(ROOT, "src")
    pngd = os.path.join(ROOT, "png")
    icod = os.path.join(ROOT, "ico")

    assets = {
        "mark.svg":       mark_svg(),
        "mark-mono.svg":  mark_svg(bg=None, p_fill=BONE, counter=None),
        "mark-ink.svg":   mark_svg(bg=INK, p_fill=BONE, counter=EMBER),
        "mark-bone.svg":  mark_svg(bg=BONE, p_fill=INK, counter=EMBER),
        "mark-flat.svg":  mark_svg(bg=None, p_fill=INK, counter=EMBER),
        "wordmark.svg":   svg(1200, 360, f'<g>{WM.element(WORD, 220, 40, 290, BONE)[0]}</g>'),
        "lockup-h.svg":   lockup_h_svg(),
        "lockup-v.svg":   lockup_v_svg(),
        "splash.svg":     splash_svg(1000, 600),
        "lockup-h-transparent.svg": lockup_h_svg(bg=None),
    }
    for name, content in assets.items():
        write(os.path.join(src, name), content)

    icon = assets["mark.svg"]
    sizes = [16, 24, 32, 48, 64, 96, 128, 256, 512, 1024]
    icon_pngs = []
    for s in sizes:
        out = os.path.join(pngd, f"partyup-icon-{s}.png")
        png(icon, out, s)
        icon_pngs.append(out)
    ico([p for p in icon_pngs if 16 <= int(p.split("-")[-1].split(".")[0]) <= 256],
        os.path.join(icod, "partyup.ico"))

    png(assets["mark-mono.svg"], os.path.join(pngd, "partyup-mark-mono-1024.png"), 1024)
    png(mark_svg(bg=None, p_fill=INK, counter=None), os.path.join(pngd, "partyup-mark-mono-ink-1024.png"), 1024)
    png(assets["mark-flat.svg"], os.path.join(pngd, "partyup-mark-flat-1024.png"), 1024)
    for name, kw in (
        ("partyup-lockup-h-ink", dict(bg=INK)),
        ("partyup-lockup-h-bone", dict(bg=BONE, text_fill=INK)),
        ("partyup-lockup-h-transparent", dict(bg=None)),
    ):
        s2 = lockup_h_svg(mark_h=256, **kw)
        png(s2, os.path.join(pngd, name + ".png"), 2000, None)
    png(splash_svg(), os.path.join(pngd, "partyup-splash-500.png"), 500, 300)
    png(splash_svg(1000, 600), os.path.join(pngd, "partyup-splash-1000.png"), 1000, 600)
    s2 = lockup_v_svg(mark_h=320, bg=INK)
    png(s2, os.path.join(pngd, "partyup-lockup-v-ink.png"), 1200, None)

    print("built:", len(assets), "svg,", len(sizes), "png,", "1 ico")


if __name__ == "__main__":
    main()
